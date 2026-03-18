#pragma once
#include <stdint.h>
#include "fb.h"

/* Blit a 64x64 palette-indexed flat onto the framebuffer.
 *
 * flat_data : 4096 bytes, each byte is a palette index (0-255).
 * palette   : first PLAYPAL palette — 256 RGB triplets (768 bytes).
 *             byte layout: R, G, B, R, G, B, ...
 * x, y      : top-left destination pixel on the framebuffer.
 * scale     : integer scale factor; each source pixel becomes scale×scale pixels.
 *             scale=8 → 512×512 output on a 1024×768 display.
 *
 * Caller is responsible for ensuring x + 64*scale <= fb->width
 * and y + 64*scale <= fb->height. No internal clipping is performed.
 */
void flat_blit(framebuffer_t *fb, const uint8_t *flat_data, const uint8_t *palette,
               uint32_t x, uint32_t y, uint32_t scale);
