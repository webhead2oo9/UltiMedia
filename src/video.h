#pragma once

#include <stdint.h>

#define FB_WIDTH 320
#define FB_HEIGHT 240

// Glyph cell width of the 8x8 font used by draw_text/draw_text_clipped;
// text measurement everywhere derives from this.
#define GLYPH_WIDTH 8

// Framebuffer pointer (allocated by core)
extern uint16_t *framebuffer;

// Initialize framebuffer
void video_init(void);

// Free framebuffer
void video_deinit(void);

// Clear framebuffer to background color
void video_clear(uint16_t bg_color);

// Fill the framebuffer with a vertical top->bottom gradient, ordered-dithered
// so RGB565 quantization does not band.
void video_fill_vgradient(uint16_t top, uint16_t bottom);

// Draw a single pixel
void draw_pixel(int x, int y, uint16_t color);

// Blend two RGB565 colors; t is 0..256 (0 = a, 256 = b)
uint16_t mix565(uint16_t a, uint16_t b, int t);

// Filled / outlined rectangles
void draw_rect_fill(int x, int y, int w, int h, uint16_t color);
void draw_rect_outline(int x, int y, int w, int h, uint16_t color);

// One-pixel bevel edge: top/left in tl, bottom/right in br.
// Raised look: tl = light, br = dark. Sunken look: swap the two.
void draw_rect_bevel(int x, int y, int w, int h, uint16_t tl, uint16_t br);

// Draw text using 8x8 font
void draw_text(int x, int y, const char* txt, uint16_t color);

// Draw text clipped to a horizontal region [clip_x, clip_x + clip_w)
void draw_text_clipped(int x, int y, const char* txt, uint16_t color, int clip_x, int clip_w);

// Integer-scaled variant of draw_text_clipped (scale 1 = 8px glyphs)
void draw_text_scaled_clipped(int x, int y, const char* txt, uint16_t color,
                              int scale, int clip_x, int clip_w);
