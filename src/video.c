#include "video.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint16_t *framebuffer = NULL;

void video_init(void) {
    framebuffer = malloc(FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
    if (!framebuffer) {
        fprintf(stderr, "[MusicCore] Failed to allocate framebuffer\n");
        return;
    }
    memset(framebuffer, 0, FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
}

void video_deinit(void) {
    free(framebuffer);
    framebuffer = NULL;
}

void video_clear(uint16_t bg_color) {
    if (!framebuffer) return;
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        framebuffer[i] = bg_color;
    }
}

void draw_pixel(int x, int y, uint16_t color) {
    if (x >= 0 && x < FB_WIDTH && y >= 0 && y < FB_HEIGHT) {
        framebuffer[y * FB_WIDTH + x] = color;
    }
}

void draw_text(int x, int y, const char* txt, uint16_t color) {
    while (*txt) {
        uint8_t c = (*txt++) - 32;
        if (c < 96) {
            for (int gy = 0; gy < 8; gy++) {
                for (int gx = 0; gx < 8; gx++) {
                    if (font8x8[c][gy] & (0x80 >> gx)) {
                        draw_pixel(x + gx, y + gy, color);
                    }
                }
            }
        }
        x += 8;
    }
}

void draw_text_clipped(int x, int y, const char* txt, uint16_t color, int clip_x, int clip_w) {
    if (!txt || clip_w <= 0) return;

    int clip_right = clip_x + clip_w;
    while (*txt) {
        uint8_t c = (*txt++) - 32;
        int char_left = x;
        int char_right = x + 8;

        if (char_right > clip_x && char_left < clip_right && c < 96) {
            for (int gy = 0; gy < 8; gy++) {
                for (int gx = 0; gx < 8; gx++) {
                    int px = x + gx;
                    if (px < clip_x || px >= clip_right) continue;
                    if (font8x8[c][gy] & (0x80 >> gx))
                        draw_pixel(px, y + gy, color);
                }
            }
        }

        x += 8;
    }
}
