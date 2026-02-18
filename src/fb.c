#include "fb.h"

static inline uint32_t pack_bgrx8888(uint8_t r, uint8_t g, uint8_t b) {
    // memory: [BB][GG][RR][XX] on little-endian
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; // 0x00RRGGBB
}

static inline void put_px32_bgrx(const framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t px) {
    *(uint32_t*)(fb->addr + y * fb->pitch + x * 4) = px;
}

bool fb_init_bgrx8888(framebuffer_t* fb, uintptr_t addr, uint32_t pitch, uint32_t w, uint32_t h, uint8_t bpp) {
    if (!fb) return false;
    if (bpp != 32) return false; // keep it strict for now

    fb->addr = (uint8_t*)addr;
    fb->pitch = pitch;
    fb->width = w;
    fb->height = h;
    fb->bpp = bpp;
    fb->fmt = FB_PIXFMT_BGRX8888;
    return true;
}

void fb_clear(framebuffer_t* fb, uint8_t r, uint8_t g, uint8_t b) {
    if (!fb || fb->fmt != FB_PIXFMT_BGRX8888) return;

    uint32_t px = pack_bgrx8888(r,g,b);
    for (uint32_t y = 0; y < fb->height; y++) {
        uint32_t* row = (uint32_t*)(fb->addr + y * fb->pitch);
        for (uint32_t x = 0; x < fb->width; x++) row[x] = px;
    }
}

void fb_fill_rect(framebuffer_t* fb, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b) {
    if (!fb || fb->fmt != FB_PIXFMT_BGRX8888) return;
    if (x0 >= fb->width || y0 >= fb->height) return;

    if (x0 + w > fb->width)  w = fb->width  - x0;
    if (y0 + h > fb->height) h = fb->height - y0;

    uint32_t px = pack_bgrx8888(r,g,b);

    for (uint32_t y = 0; y < h; y++) {
        uint32_t* row = (uint32_t*)(fb->addr + (y0 + y) * fb->pitch + x0 * 4);
        for (uint32_t x = 0; x < w; x++) row[x] = px;
    }
}

void fb_test_byte_lane_probe(framebuffer_t* fb) {
    if (!fb || fb->fmt != FB_PIXFMT_BGRX8888) return;

    // Four bars: set each byte lane to 0xFF to see which bar is which channel.
    // Expected for BGRX: bar0 blue, bar1 green, bar2 red, bar3 black/unused.
    fb_clear(fb, 0,0,0);

    uint32_t bar_h = (fb->height > 120) ? 120 : (fb->height / 4);
    if (bar_h < 40) bar_h = (fb->height / 4);

    for (uint32_t y = 0; y < bar_h; y++) {
        for (uint32_t x = 0; x < fb->width; x++) {
            uint32_t q = x / (fb->width / 4 ? (fb->width / 4) : 1);
            if (q > 3) q = 3;

            uint32_t px =
                (q == 0) ? 0x000000FFu :
                (q == 1) ? 0x0000FF00u :
                (q == 2) ? 0x00FF0000u :
                           0xFF000000u;

            *(uint32_t*)(fb->addr + y * fb->pitch + x * 4) = px;
        }
    }
}

void fb_test_color_sanity(framebuffer_t* fb) {
    if (!fb || fb->fmt != FB_PIXFMT_BGRX8888) return;

    fb_clear(fb, 0,0,0);

    // Top: 8 bars (W Y C G M R B K)
    struct { uint8_t r,g,b; } bars[8] = {
        {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
        {255,0,255},   {255,0,0},   {0,0,255},   {0,0,0}
    };

    uint32_t bar_h = fb->height / 5;
    if (bar_h < 60) bar_h = (fb->height < 60) ? fb->height : 60;
    uint32_t bar_w = fb->width / 8;

    for (uint32_t i = 0; i < 8; i++) {
        uint32_t x = i * bar_w;
        uint32_t w = (i == 7) ? (fb->width - x) : bar_w;
        fb_fill_rect(fb, x, 0, w, bar_h, bars[i].r, bars[i].g, bars[i].b);
    }

    // Middle-left: grayscale ramp
    uint32_t y0 = bar_h + 10;
    if (y0 >= fb->height) return;

    uint32_t ramp_h = (fb->height - y0) / 2;
    uint32_t ramp_w = fb->width / 2;

    for (uint32_t y = 0; y < ramp_h; y++) {
        for (uint32_t x = 0; x < ramp_w; x++) {
            uint8_t t = (uint8_t)((x * 255u) / (ramp_w ? (ramp_w - 1) : 1));
            put_px32_bgrx(fb, x, y0 + y, pack_bgrx8888(t,t,t));
        }
    }

    // Middle-right: RGB + white blocks
    uint32_t bx = ramp_w + 10;
    uint32_t by = y0;
    uint32_t sq = (ramp_h / 2) > 5 ? ((ramp_h / 2) - 5) : 0;
    if (sq > 120) sq = 120;

    if (sq > 0 && bx + sq*2 + 10 < fb->width && by + sq*2 + 10 < fb->height) {
        fb_fill_rect(fb, bx,         by,         sq, sq, 255,0,0);
        fb_fill_rect(fb, bx + sq+10, by,         sq, sq, 0,255,0);
        fb_fill_rect(fb, bx,         by + sq+10, sq, sq, 0,0,255);
        fb_fill_rect(fb, bx + sq+10, by + sq+10, sq, sq, 255,255,255);
    }

    // Bottom: tiny RGB cube sampler
    uint32_t grid_y = y0 + ramp_h + 10;
    if (grid_y >= fb->height) return;

    uint32_t cells = 6;
    uint32_t cell = fb->width / (cells + 2);
    if (cell < 20) cell = 20;

    for (uint32_t gy = 0; gy < cells; gy++) {
        for (uint32_t gx = 0; gx < cells; gx++) {
            uint8_t r = (uint8_t)((gx * 255u) / (cells - 1));
            uint8_t g = (uint8_t)((gy * 255u) / (cells - 1));
            uint8_t b = (uint8_t)(((gx ^ gy) * 255u) / (cells - 1));

            uint32_t x0 = 10 + gx * (cell + 4);
            uint32_t y1 = grid_y + gy * (cell + 4);

            if (x0 + cell < fb->width && y1 + cell < fb->height) {
                fb_fill_rect(fb, x0, y1, cell, cell, r,g,b);
            }
        }
    }
}
