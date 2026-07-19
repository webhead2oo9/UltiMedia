// Optional OpenGL presentation path (Phase A of docs/gpu-renderer-plan.md).
// The GL types, constants, and function pointers are declared locally and
// resolved through the frontend's get_proc_address so the file compiles on
// every CI platform without any GL SDK installed and links nothing new.
//
// Frame pipeline: the CPU framebuffer is uploaded once per frame, then
//   1. bright-pass into a fixed 320x240 glow target (luma threshold)
//   2. separable gaussian blur, horizontal then vertical (ping-pong FBOs)
//   3. present: passthrough quad of the scene, plus the blurred glow added
//      on top (GL_ONE/GL_ONE)
// The glow stage is optional at runtime: if its functions or FBOs are
// unavailable the passthrough still runs, so bloom failure never costs the
// picture itself.

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
#define GL_ONE 1
#define GL_FUNC_ADD 0x8006
#define GL_TRIANGLE_STRIP 0x0005
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_SWAP_BYTES 0x0CF0
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RGB 0x1907
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
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
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0

// Glow renders at the logical UI resolution regardless of media_resolution,
// so the halo width stays proportional to the chunky logical pixels.
#define GLOW_W 320
#define GLOW_H 240
#define GLOW_THRESHOLD 0.55f
#define GLOW_INTENSITY 0.3f

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
    X(glColorMask, void, (GLboolean r, GLboolean g, GLboolean b, GLboolean a)) \
    X(glBindFramebuffer, void, (GLenum target, GLuint framebuffer))

// The glow stage's extra entry points. Kept separate so a driver missing
// them only loses bloom, never the picture.
#define RGL_GLOW_PROC_LIST(X) \
    X(glGenFramebuffers, void, (GLsizei n, GLuint *framebuffers)) \
    X(glDeleteFramebuffers, void, (GLsizei n, const GLuint *framebuffers)) \
    X(glFramebufferTexture2D, void, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(glCheckFramebufferStatus, GLenum, (GLenum target)) \
    X(glBlendFunc, void, (GLenum sfactor, GLenum dfactor)) \
    X(glBlendEquation, void, (GLenum mode)) \
    X(glUniform1f, void, (GLint location, GLfloat v0)) \
    X(glUniform2f, void, (GLint location, GLfloat v0, GLfloat v1))

#define RGL_DECLARE(name, ret, args) \
    typedef ret (RGL_APIENTRY *pfn_##name) args; \
    static pfn_##name name##_;
RGL_PROC_LIST(RGL_DECLARE)
RGL_GLOW_PROC_LIST(RGL_DECLARE)
#undef RGL_DECLARE

// One compiled+linked program with its standard locations. u_a/u_b are the
// per-shader extra uniforms (threshold, blur direction, intensity).
typedef struct {
    GLuint id;
    GLint a_pos, a_uv, u_tex, u_a, u_b;
} GlProg;

static struct retro_hw_render_callback hw_cb;
static bool gl_negotiated = false;      // SET_HW_RENDER accepted this load
static bool gl_context_alive = false;   // between context_reset and destroy
static bool gl_resources_ready = false; // scene texture/blit program/VBO valid
static bool gl_glow_ready = false;      // bloom chain valid (optional)
static GLuint gl_texture = 0;
static GLuint gl_vbo = 0;
static GLuint gl_glow_tex[2] = {0, 0};
static GLuint gl_glow_fbo[2] = {0, 0};
static GlProg prog_blit;
static GlProg prog_bright;
static GlProg prog_blur;
static GlProg prog_glow;

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

static const char *blit_frag_src =
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

static const char *bright_frag_src =
    "uniform sampler2D u_tex;\n"
    "uniform float u_threshold;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec3 c = texture2D(u_tex, v_uv).rgb;\n"
    "    float luma = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "    gl_FragColor = vec4(c * smoothstep(u_threshold, u_threshold + 0.2, luma), 1.0);\n"
    "}\n";

// 9-tap gaussian using the linear-sampling trick (two texels per fetch).
static const char *blur_frag_src =
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_dir;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 o1 = u_dir * 1.3846154;\n"
    "    vec2 o2 = u_dir * 3.2307692;\n"
    "    vec3 sum = texture2D(u_tex, v_uv).rgb * 0.2270270;\n"
    "    sum += texture2D(u_tex, v_uv + o1).rgb * 0.3162162;\n"
    "    sum += texture2D(u_tex, v_uv - o1).rgb * 0.3162162;\n"
    "    sum += texture2D(u_tex, v_uv + o2).rgb * 0.0702703;\n"
    "    sum += texture2D(u_tex, v_uv - o2).rgb * 0.0702703;\n"
    "    gl_FragColor = vec4(sum, 1.0);\n"
    "}\n";

static const char *glow_frag_src =
    "uniform sampler2D u_tex;\n"
    "uniform float u_intensity;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb * u_intensity, 1.0);\n"
    "}\n";

static bool load_gl_functions(void) {
    if (!hw_cb.get_proc_address) return false;
#define RGL_LOAD(name, ret, args) \
    name##_ = (pfn_##name)hw_cb.get_proc_address(#name);
    RGL_PROC_LIST(RGL_LOAD)
#undef RGL_LOAD
    // Pre-3.0 drivers expose FBO binding only under the EXT name.
    if (!glBindFramebuffer_)
        glBindFramebuffer_ = (pfn_glBindFramebuffer)hw_cb.get_proc_address("glBindFramebufferEXT");
    // Verify only after the alias fallback so EXT-only GL2 contexts pass.
    bool ok = true;
#define RGL_CHECK(name, ret, args) if (!name##_) ok = false;
    RGL_PROC_LIST(RGL_CHECK)
#undef RGL_CHECK
    if (!ok)
        core_log(RETRO_LOG_ERROR,
                 "[MusicCore] GL renderer: required GL functions are missing; video will dupe. "
                 "Set media_renderer to Software and reload.\n");
    return ok;
}

static bool load_glow_functions(void) {
#define RGL_LOAD(name, ret, args) \
    name##_ = (pfn_##name)hw_cb.get_proc_address(#name);
    RGL_GLOW_PROC_LIST(RGL_LOAD)
#undef RGL_LOAD
    if (!glGenFramebuffers_)
        glGenFramebuffers_ = (pfn_glGenFramebuffers)hw_cb.get_proc_address("glGenFramebuffersEXT");
    if (!glDeleteFramebuffers_)
        glDeleteFramebuffers_ = (pfn_glDeleteFramebuffers)hw_cb.get_proc_address("glDeleteFramebuffersEXT");
    if (!glFramebufferTexture2D_)
        glFramebufferTexture2D_ = (pfn_glFramebufferTexture2D)hw_cb.get_proc_address("glFramebufferTexture2DEXT");
    if (!glCheckFramebufferStatus_)
        glCheckFramebufferStatus_ = (pfn_glCheckFramebufferStatus)hw_cb.get_proc_address("glCheckFramebufferStatusEXT");
    if (!glBlendEquation_)
        glBlendEquation_ = (pfn_glBlendEquation)hw_cb.get_proc_address("glBlendEquationEXT");
    bool ok = true;
#define RGL_CHECK(name, ret, args) if (!name##_) ok = false;
    RGL_GLOW_PROC_LIST(RGL_CHECK)
#undef RGL_CHECK
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

// Builds one program; u_a_name/u_b_name may be NULL when the shader has no
// extra uniforms. a_pos/a_uv/u_tex are required in every program.
static bool build_program(GlProg *p, const char *frag_src,
                          const char *u_a_name, const char *u_b_name) {
    memset(p, 0, sizeof(*p));
    p->u_a = -1;
    p->u_b = -1;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
    if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!fs) {
        glDeleteShader_(vs);
        return false;
    }

    p->id = glCreateProgram_();
    glAttachShader_(p->id, vs);
    glAttachShader_(p->id, fs);
    glLinkProgram_(p->id);
    glDeleteShader_(vs);
    glDeleteShader_(fs);

    GLint status = 0;
    glGetProgramiv_(p->id, GL_LINK_STATUS, &status);
    if (!status) {
        char info[512] = {0};
        glGetProgramInfoLog_(p->id, sizeof(info) - 1, NULL, info);
        core_log(RETRO_LOG_ERROR, "[MusicCore] GL renderer: program link failed: %s\n", info);
        glDeleteProgram_(p->id);
        p->id = 0;
        return false;
    }

    p->a_pos = glGetAttribLocation_(p->id, "a_pos");
    p->a_uv = glGetAttribLocation_(p->id, "a_uv");
    p->u_tex = glGetUniformLocation_(p->id, "u_tex");
    if (u_a_name) p->u_a = glGetUniformLocation_(p->id, u_a_name);
    if (u_b_name) p->u_b = glGetUniformLocation_(p->id, u_b_name);
    return p->a_pos >= 0 && p->a_uv >= 0 && p->u_tex >= 0;
}

static void delete_program(GlProg *p) {
    if (p->id && glDeleteProgram_) glDeleteProgram_(p->id);
    p->id = 0;
}

// Draw a fullscreen quad with the given UV extent. The FBO is bottom-left
// origin while the CPU framebuffer is top-left, so the final present flips V
// in the UVs (never in the upload); offscreen glow passes stay unflipped so
// their textures keep the CPU orientation throughout.
static void draw_quad(const GlProg *p, GLfloat umax, GLfloat vmax, bool flip) {
    GLfloat v_bot = flip ? vmax : 0.0f;
    GLfloat v_top = flip ? 0.0f : vmax;
    GLfloat verts[16] = {
        -1.0f, -1.0f, 0.0f, v_bot,
         1.0f, -1.0f, umax, v_bot,
        -1.0f,  1.0f, 0.0f, v_top,
         1.0f,  1.0f, umax, v_top,
    };
    glBindBuffer_(GL_ARRAY_BUFFER, gl_vbo);
    glBufferData_(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray_((GLuint)p->a_pos);
    glEnableVertexAttribArray_((GLuint)p->a_uv);
    glVertexAttribPointer_((GLuint)p->a_pos, 2, GL_FLOAT, 0,
                           4 * (GLsizei)sizeof(GLfloat), (const void*)0);
    glVertexAttribPointer_((GLuint)p->a_uv, 2, GL_FLOAT, 0,
                           4 * (GLsizei)sizeof(GLfloat), (const void*)(2 * sizeof(GLfloat)));
    glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray_((GLuint)p->a_pos);
    glDisableVertexAttribArray_((GLuint)p->a_uv);
    glBindBuffer_(GL_ARRAY_BUFFER, 0);
}

static bool build_resources(void) {
    // Allocate the scene texture at the declared max once; per-frame uploads
    // then touch only the active region, and resolution changes need no
    // realloc.
    glGenTextures_(1, &gl_texture);
    glBindTexture_(GL_TEXTURE_2D, gl_texture);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGB, FB_MAX_WIDTH, FB_MAX_HEIGHT, 0,
                  GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);

    if (!build_program(&prog_blit, blit_frag_src, NULL, NULL)) return false;

    glGenBuffers_(1, &gl_vbo);

    return glGetError_() == GL_NO_ERROR;
}

static bool build_glow_resources(void) {
    if (!load_glow_functions()) return false;

    if (!build_program(&prog_bright, bright_frag_src, "u_threshold", NULL)) return false;
    if (!build_program(&prog_blur, blur_frag_src, "u_dir", NULL)) return false;
    if (!build_program(&prog_glow, glow_frag_src, "u_intensity", NULL)) return false;
    if (prog_bright.u_a < 0 || prog_blur.u_a < 0 || prog_glow.u_a < 0) return false;

    for (int i = 0; i < 2; i++) {
        glGenTextures_(1, &gl_glow_tex[i]);
        glBindTexture_(GL_TEXTURE_2D, gl_glow_tex[i]);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGB, GLOW_W, GLOW_H, 0,
                      GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers_(1, &gl_glow_fbo[i]);
        glBindFramebuffer_(GL_FRAMEBUFFER, gl_glow_fbo[i]);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, gl_glow_tex[i], 0);
        if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer_(GL_FRAMEBUFFER, 0);
            return false;
        }
    }
    // Leave nothing of ours bound when context_reset returns -- frontend GL
    // work before the first frame must not land in a glow FBO.
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);

    return glGetError_() == GL_NO_ERROR;
}

static void context_reset_cb(void) {
    // Per the libretro contract, everything GPU-side is invalid here; new
    // names are created without freeing the old ones.
    gl_context_alive = true;
    gl_resources_ready = false;
    gl_glow_ready = false;
    gl_texture = 0;
    gl_vbo = 0;
    gl_glow_tex[0] = gl_glow_tex[1] = 0;
    gl_glow_fbo[0] = gl_glow_fbo[1] = 0;
    memset(&prog_blit, 0, sizeof(prog_blit));
    memset(&prog_bright, 0, sizeof(prog_bright));
    memset(&prog_blur, 0, sizeof(prog_blur));
    memset(&prog_glow, 0, sizeof(prog_glow));

    if (!load_gl_functions()) return;
    // A shared context can arrive with a stale error flag left by the
    // frontend; drain it so build_resources' final check only sees errors
    // raised by this setup. Bounded: drivers queue at most a few flags.
    for (int i = 0; i < 16 && glGetError_() != GL_NO_ERROR; i++) {}
    if (!build_resources()) {
        core_log(RETRO_LOG_ERROR,
                 "[MusicCore] GL renderer: resource setup failed; video will dupe. "
                 "Set media_renderer to Software and reload.\n");
        return;
    }
    gl_resources_ready = true;

    gl_glow_ready = build_glow_resources();
    glBindTexture_(GL_TEXTURE_2D, 0);
    if (gl_glow_ready)
        core_log(RETRO_LOG_INFO, "[MusicCore] GL renderer: context ready (glow enabled).\n");
    else
        core_log(RETRO_LOG_WARN,
                 "[MusicCore] GL renderer: glow setup failed; passthrough only.\n");
}

static void context_destroy_cb(void) {
    // Delete whatever exists, not just fully-built generations -- with
    // cache_context a partially-built set would otherwise outlive the
    // session inside the shared context.
    if (gl_texture && glDeleteTextures_) glDeleteTextures_(1, &gl_texture);
    if (gl_vbo && glDeleteBuffers_) glDeleteBuffers_(1, &gl_vbo);
    for (int i = 0; i < 2; i++) {
        if (gl_glow_tex[i] && glDeleteTextures_) glDeleteTextures_(1, &gl_glow_tex[i]);
        if (gl_glow_fbo[i] && glDeleteFramebuffers_) glDeleteFramebuffers_(1, &gl_glow_fbo[i]);
        gl_glow_tex[i] = 0;
        gl_glow_fbo[i] = 0;
    }
    delete_program(&prog_blit);
    delete_program(&prog_bright);
    delete_program(&prog_blur);
    delete_program(&prog_glow);
    gl_texture = 0;
    gl_vbo = 0;
    gl_resources_ready = false;
    gl_glow_ready = false;
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

bool render_gl_negotiated(void) {
    return gl_negotiated;
}

bool render_gl_frame(void) {
    if (!gl_negotiated || !gl_context_alive || !gl_resources_ready) return false;
    if (!hw_cb.get_current_framebuffer) return false;

    GLfloat umax = (GLfloat)fb_width / (GLfloat)FB_MAX_WIDTH;
    GLfloat vmax = (GLfloat)fb_height / (GLfloat)FB_MAX_HEIGHT;

    glDisable_(GL_DEPTH_TEST);
    glDisable_(GL_CULL_FACE);
    glDisable_(GL_BLEND);
    glDisable_(GL_SCISSOR_TEST);
    glColorMask_(1, 1, 1, 1);

    // EmuVR runs with a shared GL context, so pixel-store state can be
    // anything the frontend left behind; pin every knob the upload reads.
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture_(GL_TEXTURE_2D, gl_texture);
    glPixelStorei_(GL_UNPACK_ALIGNMENT, 2);
    glPixelStorei_(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei_(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei_(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei_(GL_UNPACK_SWAP_BYTES, 0);
    glTexSubImage2D_(GL_TEXTURE_2D, 0, 0, 0, fb_width, fb_height,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5, framebuffer);

    if (gl_glow_ready) {
        // Bright-pass: LINEAR so downsampling to the glow target averages
        // instead of dropping pixels at high resolutions.
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindFramebuffer_(GL_FRAMEBUFFER, gl_glow_fbo[0]);
        glViewport_(0, 0, GLOW_W, GLOW_H);
        glUseProgram_(prog_bright.id);
        glUniform1i_(prog_bright.u_tex, 0);
        glUniform1f_(prog_bright.u_a, GLOW_THRESHOLD);
        draw_quad(&prog_bright, umax, vmax, false);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        // Separable blur, horizontal into fbo[1] then vertical back into
        // fbo[0].
        glUseProgram_(prog_blur.id);
        glUniform1i_(prog_blur.u_tex, 0);
        glBindFramebuffer_(GL_FRAMEBUFFER, gl_glow_fbo[1]);
        glBindTexture_(GL_TEXTURE_2D, gl_glow_tex[0]);
        glUniform2f_(prog_blur.u_a, 1.0f / (GLfloat)GLOW_W, 0.0f);
        draw_quad(&prog_blur, 1.0f, 1.0f, false);
        glBindFramebuffer_(GL_FRAMEBUFFER, gl_glow_fbo[0]);
        glBindTexture_(GL_TEXTURE_2D, gl_glow_tex[1]);
        glUniform2f_(prog_blur.u_a, 0.0f, 1.0f / (GLfloat)GLOW_H);
        draw_quad(&prog_blur, 1.0f, 1.0f, false);
    }

    // Present into the frontend's FBO: crisp scene, then additive glow.
    glBindFramebuffer_(GL_FRAMEBUFFER, (GLuint)hw_cb.get_current_framebuffer());
    glViewport_(0, 0, fb_width, fb_height);
    glClearColor_(0.0f, 0.0f, 0.0f, 1.0f);
    glClear_(GL_COLOR_BUFFER_BIT);
    glUseProgram_(prog_blit.id);
    glUniform1i_(prog_blit.u_tex, 0);
    glBindTexture_(GL_TEXTURE_2D, gl_texture);
    draw_quad(&prog_blit, umax, vmax, true);

    if (gl_glow_ready) {
        glEnable_(GL_BLEND);
        glBlendEquation_(GL_FUNC_ADD); // shared state could hold subtract/min/max
        glBlendFunc_(GL_ONE, GL_ONE);
        glUseProgram_(prog_glow.id);
        glUniform1i_(prog_glow.u_tex, 0);
        glUniform1f_(prog_glow.u_a, GLOW_INTENSITY);
        glBindTexture_(GL_TEXTURE_2D, gl_glow_tex[0]);
        draw_quad(&prog_glow, 1.0f, 1.0f, true);
        glDisable_(GL_BLEND);
    }

    glUseProgram_(0);
    // Unbind owned objects so nothing leaks into the frontend's pass in the
    // shared context.
    glBindTexture_(GL_TEXTURE_2D, 0);
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    return true;
}

void render_gl_shutdown(void) {
    gl_negotiated = false;
}
