#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    FB_PIXFMT_UNKNOWN = 0,
    FB_PIXFMT_BGRX8888, // byte0=B, byte1=G, byte2=R, byte3=unused
} fb_pixfmt_t;

typedef struct {
    uint8_t*  addr;   // linear framebuffer base (phys-mapped identity right now)
    uint32_t  pitch;  // bytes per scanline
    uint32_t  width;
    uint32_t  height;
    uint8_t   bpp;    // bits per pixel
    fb_pixfmt_t fmt;
} framebuffer_t;

// Init using the format we empirically detected in QEMU/GRUB: BGRX8888
// Returns false if unsupported (e.g., not 32bpp)
bool fb_init_bgrx8888(framebuffer_t* fb, uintptr_t addr, uint32_t pitch, uint32_t w, uint32_t h, uint8_t bpp);

// Basic drawing
void fb_clear(framebuffer_t* fb, uint8_t r, uint8_t g, uint8_t b);
void fb_fill_rect(framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b);

// Visual tests
void fb_test_color_sanity(framebuffer_t* fb);

// Optional: pixel-format probe (useful when you move beyond QEMU)
void fb_test_byte_lane_probe(framebuffer_t* fb);
