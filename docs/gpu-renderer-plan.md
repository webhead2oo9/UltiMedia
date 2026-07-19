# GPU Renderer Plan (OpenGL `SET_HW_RENDER`)

Design plan for an optional OpenGL backend. The software renderer remains the
default and the canonical implementation; GL is an opt-in enhancement layer.

## Why, and why this shape

- **Goal:** effects the software path cannot do — real alpha, additive glow and
  bloom, smooth high-resolution 3D (Horizon as shaded geometry, particle
  modes) — without sacrificing the core's universal compatibility.
- **EmuVR reality (verified against the local install):** EmuVR runs RetroArch
  1.7.5 with `video_driver = "gl"` and `video_shared_context = "true"`, and
  ships GL hw_render cores (ParaLLEl-N64), so `RETRO_HW_CONTEXT_OPENGL` is
  viable. Vulkan is not. Frames reach EmuVR via a Spout shared texture, so
  anything that reinitializes the video driver mid-session is off-limits
  (same rule as the resolution work: `SET_GEOMETRY` yes, `SET_SYSTEM_AV_INFO`
  no).
- **Software stays default:** RetroArch users on the d3d driver cannot run
  GL-only cores, the native test harness and CI are software-only, and the
  chunky RGB565 identity is the brand. GL must never be required.

## Phased approach

**Phase A — hybrid post-processing (the meat of this branch).**
Keep the entire existing software pipeline untouched: `ui.c`/`visualizer.c`
render into the CPU framebuffer exactly as today. In GL mode, upload that
buffer as a texture each frame (`GL_UNSIGNED_SHORT_5_6_5`) and draw a
fullscreen quad through post shaders: additive glow/bloom (bright-pass +
separable blur + composite) tuned for the LED/phosphor elements. One code
path remains the source of truth for composition; GL adds light on top.

**Phase B — native GL layers where they pay.**
Selected visualizer modes gain GL-native implementations layered over the
software UI: Horizon as depth-tested shaded triangle strips, Scope with
additive line glow, possibly a particle mode. Each mode keeps its software
version; GL variants are extra.

**Phase C — full GL UI. Deliberately unscheduled.**
Only if Phase B proves the maintenance cost is worth it.

## Mechanics

- **Negotiation:** `retro_hw_render_callback` with `RETRO_HW_CONTEXT_OPENGL`
  requested in `retro_load_game` **only when** the `media_renderer` option is
  `OpenGL`; if the frontend refuses, fall back to software silently. Render
  into the frontend-provided FBO (`get_current_framebuffer`), pass
  `RETRO_HW_FRAME_BUFFER_VALID` to the video callback.
- **Context loss is normal:** all GPU objects (texture, shaders, VBO) are
  rebuilt in `context_reset` and dropped in `context_destroy`; CPU-side state
  is always sufficient to rebuild. No GL calls outside `retro_run`.
- **Function loading:** a minimal hand-rolled loader over
  `get_proc_address` (no new dependencies). GLSL kept to a conservative
  version for the `gl` driver.
- **New files:** `src/render_gl.c` / `render_gl.h` (context lifecycle, upload,
  shaders as embedded strings, post pass). `core.c` gains the negotiation and
  a per-frame "software buffer vs GL frame" fork. Build lists (run_tests.py,
  build scripts, CI, README/CONTRIBUTING/CLAUDE.md commands) gain the file.
- **Options:** `media_renderer` (`Software` | `OpenGL`), default `Software`.
  Later: `media_glow` intensity presets once Phase A works.
- **Origin/orientation:** GL FBO is bottom-left origin — flip in the quad UVs,
  not in the upload.

## Testing & risk

- Harness/CI stay software-only and must remain green; `render_gl.c` must
  compile on all three CI OSes even though CI never executes GL.
- Manual verification: desktop RetroArch with the `gl` driver first, then
  EmuVR (watch the Spout handoff and `video_shared_context`).
- Riskiest unknowns: EmuVR's fork handling a hw_render core it has no
  override record for; context churn when EmuVR juggles multiple consoles.
  Both need live experiments early in Phase A.

## Definition of done (this branch)

Phase A complete: `media_renderer = OpenGL` produces the identical
composition plus glow/bloom, survives context loss, falls back cleanly when
GL is unavailable, and EmuVR runs it without regressions at every
`media_resolution` preset. Software default remains byte-identical to main.
