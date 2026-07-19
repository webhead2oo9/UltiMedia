#include "video.h"
#include "font.h"
#include "core_log.h"
#include <stdlib.h>
#include <string.h>

uint16_t *framebuffer = NULL;
int fb_width = 320;
int fb_height = 240;

void video_init(void) {
    // Allocate at the maximum so resolution switches never reallocate.
    framebuffer = malloc(FB_MAX_WIDTH * FB_MAX_HEIGHT * sizeof(uint16_t));
    if (!framebuffer) {
        core_log(RETRO_LOG_ERROR, "[MusicCore] Failed to allocate framebuffer\n");
        return;
    }
    memset(framebuffer, 0, FB_MAX_WIDTH * FB_MAX_HEIGHT * sizeof(uint16_t));
}

void video_set_resolution(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > FB_MAX_WIDTH) w = FB_MAX_WIDTH;
    if (h > FB_MAX_HEIGHT) h = FB_MAX_HEIGHT;
    fb_width = w;
    fb_height = h;
    if (framebuffer) memset(framebuffer, 0, FB_MAX_WIDTH * FB_MAX_HEIGHT * sizeof(uint16_t));
}

void video_deinit(void) {
    free(framebuffer);
    framebuffer = NULL;
}

void video_clear(uint16_t bg_color) {
    if (!framebuffer) return;
    for (int i = 0; i < fb_width * fb_height; i++) {
        framebuffer[i] = bg_color;
    }
}

void video_fill_vgradient(uint16_t top, uint16_t bottom) {
    // 4x4 Bayer matrix; offsets scaled to one RGB565 quantum per channel.
    static const uint8_t bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}
    };
    if (!framebuffer) return;

    int tr = ((top >> 11) & 0x1F) << 3, tg = ((top >> 5) & 0x3F) << 2, tb = (top & 0x1F) << 3;
    int br = ((bottom >> 11) & 0x1F) << 3, bg = ((bottom >> 5) & 0x3F) << 2, bb = (bottom & 0x1F) << 3;

    for (int y = 0; y < fb_height; y++) {
        int r8 = tr + ((br - tr) * y) / (fb_height - 1);
        int g8 = tg + ((bg - tg) * y) / (fb_height - 1);
        int b8 = tb + ((bb - tb) * y) / (fb_height - 1);
        uint16_t *row = framebuffer + y * fb_width;
        for (int x = 0; x < fb_width; x++) {
            int d = bayer[y & 3][x & 3];
            int r = (r8 + (d >> 1)) >> 3;
            int g = (g8 + (d >> 2)) >> 2;
            int b = (b8 + (d >> 1)) >> 3;
            if (r > 31) r = 31;
            if (g > 63) g = 63;
            if (b > 31) b = 31;
            row[x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

void draw_pixel(int x, int y, uint16_t color) {
    if (framebuffer && x >= 0 && x < fb_width && y >= 0 && y < fb_height) {
        framebuffer[y * fb_width + x] = color;
    }
}

uint16_t mix565(uint16_t a, uint16_t b, int t) {
    if (t <= 0) return a;
    if (t >= 256) return b;
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (((br - ar) * t) >> 8);
    int g = ag + (((bg - ag) * t) >> 8);
    int bl = ab + (((bb - ab) * t) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

void draw_rect_fill(int x, int y, int w, int h, uint16_t color) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) draw_pixel(x + i, y + j, color);
    }
}

void draw_rect_outline(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    int x2 = x + w - 1;
    int y2 = y + h - 1;
    for (int px = x; px <= x2; px++) {
        draw_pixel(px, y, color);
        draw_pixel(px, y2, color);
    }
    for (int py = y; py <= y2; py++) {
        draw_pixel(x, py, color);
        draw_pixel(x2, py, color);
    }
}

void draw_rect_bevel(int x, int y, int w, int h, uint16_t tl, uint16_t br) {
    if (w <= 0 || h <= 0) return;
    int x2 = x + w - 1;
    int y2 = y + h - 1;
    for (int px = x; px <= x2; px++) {
        draw_pixel(px, y, tl);
        draw_pixel(px, y2, br);
    }
    for (int py = y; py <= y2; py++) {
        draw_pixel(x, py, tl);
        draw_pixel(x2, py, br);
    }
    // Top-right corner belongs to the top edge, bottom-left to the bottom.
    draw_pixel(x2, y, tl);
    draw_pixel(x, y2, br);
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
        x += GLYPH_WIDTH;
    }
}

void draw_text_clipped(int x, int y, const char* txt, uint16_t color, int clip_x, int clip_w) {
    draw_text_scaled_clipped(x, y, txt, color, 1, clip_x, clip_w);
}

void draw_text_scaled_clipped(int x, int y, const char* txt, uint16_t color,
                              int scale, int clip_x, int clip_w) {
    if (!txt || clip_w <= 0 || scale < 1) return;

    int cell = GLYPH_WIDTH * scale;
    int clip_right = clip_x + clip_w;
    while (*txt) {
        uint8_t c = (*txt++) - 32;

        if (x + cell > clip_x && x < clip_right && c < 96) {
            for (int gy = 0; gy < 8; gy++) {
                for (int gx = 0; gx < 8; gx++) {
                    if (!(font8x8[c][gy] & (0x80 >> gx))) continue;
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            int px = x + gx * scale + sx;
                            if (px < clip_x || px >= clip_right) continue;
                            draw_pixel(px, y + gy * scale + sy, color);
                        }
                    }
                }
            }
        }

        x += cell;
    }
}
