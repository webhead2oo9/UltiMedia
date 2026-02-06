#pragma once

#include <stdint.h>

#define FB_WIDTH 320
#define FB_HEIGHT 240

// Framebuffer pointer (allocated by core)
extern uint16_t *framebuffer;

// Initialize framebuffer
void video_init(void);

// Free framebuffer
void video_deinit(void);

// Clear framebuffer to background color
void video_clear(uint16_t bg_color);

// Draw a single pixel
void draw_pixel(int x, int y, uint16_t color);

// Draw text using 8x8 font
void draw_text(int x, int y, const char* txt, uint16_t color);
