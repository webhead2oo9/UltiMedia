#include "video.h"
#include "font.h"
#include <stdlib.h>
#include <string.h>

uint16_t *framebuffer = NULL;

void video_init(void) {
    framebuffer = malloc(FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
    if (framebuffer) {
        memset(framebuffer, 0, FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
    }
}

void video_deinit(void) {
    free(framebuffer);
    framebuffer = NULL;
}

void video_clear(uint16_t bg_color) {
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        framebuffer[i] = bg_color;
    }
}

void video_apply_lcd_effect(void) {
    for (int y = 0; y < FB_HEIGHT; y += 2) {
        for (int x = 0; x < FB_WIDTH; x++) {
            framebuffer[y * FB_WIDTH + x] = (framebuffer[y * FB_WIDTH + x] >> 1) & 0x7BEF;
        }
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
