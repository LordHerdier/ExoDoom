#include "flat.h"
#include "fb.h"

void flat_blit(framebuffer_t *fb, const uint8_t *flat_data, const uint8_t *palette,
               uint32_t x, uint32_t y, uint32_t scale) {
    for (uint32_t row = 0; row < 64; row++) {
        for (uint32_t col = 0; col < 64; col++) {
            uint8_t idx = flat_data[row * 64 + col];
            uint8_t r   = palette[idx * 3 + 0];
            uint8_t g   = palette[idx * 3 + 1];
            uint8_t b   = palette[idx * 3 + 2];
            fb_fill_rect(fb, x + col * scale, y + row * scale, scale, scale, r, g, b);
        }
    }
}
