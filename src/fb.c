#include "fb.h"

static inline uint32_t pack_bgrx8888(uint8_t r, uint8_t g, uint8_t b) {
    // memory: [BB][GG][RR][XX] on little-endian
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; // 0x00RRGGBB
}

static inline void put_px32_bgrx(const framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t px) {
    *(uint32_t*)(fb->addr + y * fb->pitch + x * 4) = px;
}

static uint8_t g_backbuffer[1024 * 768 * 4];

bool fb_init_bgrx8888(framebuffer_t* fb, uintptr_t addr, uint32_t pitch, uint32_t w, uint32_t h, uint8_t bpp) {
    if (!fb) return false;
    if (bpp != 32) return false; // keep it strict for now

    fb->addr = (uint8_t*)addr;
    fb->pitch = pitch;
    fb->width = w;
    fb->height = h;
    fb->bpp = bpp;
    fb->fmt = FB_PIXFMT_BGRX8888;
    fb->backbuffer = g_backbuffer; // Point to our RAM buffer
    
    // Clear backbuffer to avoid garbage on startup
    for(uint32_t i=0; i<w*h*4; i++) fb->backbuffer[i] = 0;
    
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
    if (!fb) return;
    
    // Safety clipping
    if (x0 >= fb->width || y0 >= fb->height) return;
    if (x0 + w > fb->width)  w = fb->width  - x0;
    if (y0 + h > fb->height) h = fb->height - y0;

    uint32_t px = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;

    // Use backbuffer instead of addr
    for (uint32_t y = 0; y < h; y++) {
        uint32_t* row = (uint32_t*)(fb->backbuffer + (y0 + y) * fb->pitch + x0 * 4);
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

static inline int i_abs(int v) { return v < 0 ? -v : v; }

void fb_line(framebuffer_t* fb, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
    if (!fb) return;

    // Bresenham's Line Algorithm
    // This draws a line without using floating point math (no decimals!)
    
    int dx = i_abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -i_abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    
    while (1) {
        // Draw the pixel at (x0, y0)
        // We use your existing 1x1 fill_rect as a safe "put_pixel"
        fb_fill_rect(fb, x0, y0, 1, 1, r, g, b);

        if (x0 == x1 && y0 == y1) break;
        
        int e2 = 2 * err;
        
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void fb_triangle(framebuffer_t* fb, int x0, int y0, int x1, int y1, int x2, int y2, uint8_t r, uint8_t g, uint8_t b) {
    // A triangle is just 3 lines connecting the 3 points
    fb_line(fb, x0, y0, x1, y1, r, g, b);
    fb_line(fb, x1, y1, x2, y2, r, g, b);
    fb_line(fb, x2, y2, x0, y0, r, g, b);
}

// Add to fb.c

// Helpers for min/max without <algorithm>
static inline int min3(int a, int b, int c) {
    int m = a < b ? a : b;
    return m < c ? m : c;
}

static inline int max3(int a, int b, int c) {
    int m = a > b ? a : b;
    return m > c ? m : c;
}

// Calculates the cross product (2D determinant)
// This is mathematically equivalent to "Signed Triangle Area * 2"
// We use this because it keeps everything as integers.
static inline int edge_cross(int ax, int ay, int bx, int by, int px, int py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void fb_triangle_filled(framebuffer_t* fb, vertex_t v0, vertex_t v1, vertex_t v2) {
    if (!fb) return;

    // 1. Calculate Bounding Box
    // We only iterate over pixels that *might* be inside the triangle
    int min_x = min3(v0.x, v1.x, v2.x);
    int min_y = min3(v0.y, v1.y, v2.y);
    int max_x = max3(v0.x, v1.x, v2.x);
    int max_y = max3(v0.y, v1.y, v2.y);

    // Clip against screen bounds (prevent writing into memory outside framebuffer)
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= (int)fb->width) max_x = fb->width - 1;
    if (max_y >= (int)fb->height) max_y = fb->height - 1;

    // 2. Calculate Total Area (actually 2x Area, but the ratio is the same)
    // Using standard cross product: (B-A) x (C-A)
    int area = edge_cross(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);

    // Back-face culling: if area is negative or zero, the triangle is facing away or degenerate
    // (Depending on your winding order, you might need to flip this check)
    if (area <= 0) return; 

    // 3. Iterate over the bounding box
    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            
            // Calculate sub-areas (weights)
            // w0 is the weight for v2 (opp side v0-v1) -- Standard barycentric naming can vary
            // Let's stick to the logic: 
            // w0 = area defined by P, v1, v2
            // w1 = area defined by v0, P, v2
            // w2 = area defined by v0, v1, P
            
            // Actually, simpler logic:
            // w0 corresponds to v0, so it's the area of triangle (v1, v2, p)
            int w0 = edge_cross(v1.x, v1.y, v2.x, v2.y, x, y);
            int w1 = edge_cross(v2.x, v2.y, v0.x, v0.y, x, y);
            int w2 = edge_cross(v0.x, v0.y, v1.x, v1.y, x, y);

            // If all weights are positive, the point is inside
            // (Using >= 0 ensures we don't have gaps between adjacent triangles)
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                
                // Interpolate Color
                // Result = (w0*c0 + w1*c1 + w2*c2) / TotalArea
                // We use integers, so we sum first, then divide.
                
                // Note: w0+w1+w2 should equal area.
                
                int r = (w0 * v0.r + w1 * v1.r + w2 * v2.r) / area;
                int g = (w0 * v0.g + w1 * v1.g + w2 * v2.g) / area;
                int b = (w0 * v0.b + w1 * v1.b + w2 * v2.b) / area;

                // Clamp colors just in case (though math says they should be safe)
                // (Omitted for speed, but good for safety)

                // Plot the pixel
                // We inline the plot function here for speed or use a helper
                // Assuming BGRX format:
                uint32_t offset = y * fb->pitch + x * 4;
                *(uint32_t*)(fb->addr + offset) = 
                    ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }
}

void fb_swap_buffers(framebuffer_t* fb) {
    // Simple memcpy equivalent
    // Ideally use an optimized assembly version for speed
    uint32_t* src = (uint32_t*)fb->backbuffer;
    uint32_t* dst = (uint32_t*)fb->addr;
    uint32_t  len = (fb->width * fb->height); 
    
    for (uint32_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }
}