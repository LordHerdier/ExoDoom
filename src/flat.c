#include "flat.h"

void flat_blit(framebuffer_t *fb, const uint8_t *flat_data, const uint8_t *palette,
               uint32_t x, uint32_t y, uint32_t scale) {
    /* Precompute packed BGRX values for all 256 palette entries.
       Avoids re-packing the color on every pixel and eliminates the
       fb_fill_rect call overhead (bounds check + function call × 4096). */
    uint32_t pal_bgrx[256];
    for (int i = 0; i < 256; i++) {
        uint8_t r = palette[i * 3 + 0];
        uint8_t g = palette[i * 3 + 1];
        uint8_t b = palette[i * 3 + 2];
        pal_bgrx[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    for (uint32_t row = 0; row < 64; row++) {
        const uint8_t *src = flat_data + row * 64;
        for (uint32_t sr = 0; sr < scale; sr++) {
            uint32_t *dst = (uint32_t *)(fb->addr + (y + row * scale + sr) * fb->pitch) + x;
            for (uint32_t col = 0; col < 64; col++) {
                uint32_t px = pal_bgrx[src[col]];
                for (uint32_t sc = 0; sc < scale; sc++)
                    *dst++ = px;
            }
        }
    }
}
