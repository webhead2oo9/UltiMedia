#pragma once

#include <stdbool.h>
#include "libretro.h"

// Optional OpenGL presentation path (SET_HW_RENDER). The software renderer
// in video.c stays canonical: ui.c/visualizer.c always compose into the CPU
// framebuffer, and this module only uploads that image and draws it through
// GL. Negotiation happens once per content load; everything else reacts to
// the frontend's context_reset/context_destroy callbacks.

// Ask the frontend for an OpenGL context. Call from retro_load_game only.
// Returns false (and logs) when the frontend refuses; the core then keeps
// presenting through the software video callback.
bool render_gl_request_context(retro_environment_t environ_cb);

// True while an OpenGL context is negotiated for the current content load.
// Once negotiation succeeds, the LibRetro contract forbids software frames:
// the video callback must receive RETRO_HW_FRAME_BUFFER_VALID or NULL (frame
// dupe) -- 1.7.5's gl driver ignores CPU uploads once its hw FBO is active.
bool render_gl_negotiated(void);

// Upload the CPU framebuffer and draw it into the frontend's FBO. Call from
// retro_run only. Returns true when a GL frame was produced (pass
// RETRO_HW_FRAME_BUFFER_VALID to the video callback); false when GL is
// inactive or unavailable (present the software framebuffer instead).
bool render_gl_frame(void);

// Forget the negotiation for this session. GL resources are not touched --
// the frontend owns their lifecycle through context_destroy.
void render_gl_shutdown(void);
